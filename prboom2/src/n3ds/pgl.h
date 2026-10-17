//
// [3DS] pgl: OpenGL 1.x on the 3DS GPU (the PICA200), through citro3d.
//
// The 3DS GPU does not speak OpenGL: citro3d drives it with its own calls.
// pgl implements the part of OpenGL 1.3 that DSDA-Doom's renderer uses on
// top of citro3d, so that renderer runs unchanged: the gl* calls build
// vertex lists, matrices and states as OpenGL describes, and pgl turns them
// into PICA200 commands, sent in one list per frame.
//
// What the PICA200 cannot do the OpenGL way, pgl adapts:
// - Lines and points: no such primitives; drawn as thin quads.
// - Textures: only power-of-two sizes from 8 to 1024; others are resampled
//   to one (GL_ARB_texture_non_power_of_two is offered on top of that).
// - Depth: the PICA200 runs it from -1 to 0, reversed; pgl converts the
//   projection and the depth tests.
// - The top screen is mounted sideways: everything is drawn rotated.
// - No GLSL: the programs below stand in for the few shaders a game needs,
//   built from the PICA200's own color combiners and fog.
//

#ifndef __N3DS_PGL__
#define __N3DS_PGL__

#include <stdbool.h>

// SDL_GL_CreateContext, SDL_GL_DeleteContext, SDL_GL_SwapWindow: the top
// screen, 400x240.
bool pglCreateContext(void);
void pglDeleteContext(void);
void pglSwapBuffers(void);
void *pglGetProcAddress(const char *name);

// Called in pglSwapBuffers, before the frame is shown: the last draws
// (e.g. a full-screen tint).
void pglSetBeforeSwap(void (*callback)(void));

// Instead of GLSL shaders. The fixed function (glTexEnv) by default.
enum
{
  PGL_PROGRAM_FIXED,
  // Texture times color, whatever glTexEnv says, then darkened with the
  // depth by the fog table (pglFogTable): the Doom lighting.
  PGL_PROGRAM_LIGHT,
  // Color, with half the texture's alpha: a see-through shadow.
  PGL_PROGRAM_SHADOW,
};

void pglProgram(int program);

// How bright things are at each depth, for PGL_PROGRAM_LIGHT: 129 values
// from 0 (black) to 1, for window depths 0, 1/128, ..., 1 (glDepthRange's
// 0 to 1, as gl_FragCoord.z). Made once, used many times.
typedef struct pgl_fog_table_s pgl_fog_table_t;

pgl_fog_table_t *pglNewFogTable(const float brightness[129]);
void pglFogTable(pgl_fog_table_t *table);

// New textures in 16 bits (RGBA5551, or RGBA4 for alphas in between) instead
// of 32: half the memory and of the GPU's reading, a little less color.
void pglTextures16(bool on);

// A texture cache: the bound texture (just made) can go when the memory is
// short, its owner making it again; and whether a texture is still there.
void pglTextureEvictable(void);
bool pglTextureResident(GLuint name);

// Blends a color over the whole screen (alpha: how much), whatever the
// OpenGL states: e.g. the red of pain, in pglSetBeforeSwap's callback.
void pglBlendScreen(float red, float green, float blue, float alpha);

// The last frame's times, in microseconds: the CPU's work (from swap to
// swap, without waiting for the GPU or the screen) and the GPU's drawing.
void pglFrameTimes(int *work_us, int *gpu_us);

// Counts since the last call (then cleared), for the log: time drawing (in
// pgl: states and draws), waiting for the GPU and the screen, uploading
// textures; and how many draws, texture binds, light (fog) changes and
// combiner changes.
typedef struct
{
  double draw_ms, wait_ms, upload_ms;
  double end_ms; // sending the frame to the GPU (the cache flush with it)
  int draws, binds, fog_changes, texenv_changes, uploads;
} pgl_stats_t;

void pglTakeStats(pgl_stats_t *stats);

#endif
