//
// [3DS] DSDA-Doom's OpenGL renderer on pgl (n3ds/pgl.h). See n3ds_gl.h.
//
// DSDA-Doom draws with OpenGL much as it draws in software: textures keep
// palette indices, and a GLSL shader looks each pixel's color up in the
// COLORMAP, darker with the distance, as Doom does. The 3DS GPU has no
// shaders for pixels, and cannot look one texture up with another, so here:
// - Textures get their colors when they are made (the normal palette).
// - The darkening with the distance is the GPU's fog: a table per light
//   level with the shader's formula, from the window depth.
// - The palettes of pain and items are a tint over the finished frame
//   (they mix the colors toward red, gold or green).
// - Spectres are half-transparent shadows.
// This file replaces gl_shader.c on the 3DS.
//

#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "doomstat.h"
#include "lprintf.h"
#include "v_video.h"
#include "w_wad.h"
#include "gl_opengl.h"
#include "gl_intern.h"

#include "n3ds/n3ds.h"
#include "n3ds/n3ds_gl.h"

// ==========================================================================
// Palettes

static int current_palette;

void N3DS_GLSetPalette(int palette)
{
  current_palette = palette;
}

static const byte *GammaTable(void)
{
  const int lump = W_CheckNumForName2("GAMMATBL", ns_prboom);

  return (const byte *) W_LumpByNum(lump) + 256 * usegamma;
}

unsigned char *N3DS_GLIndexedToRGBA(const unsigned char *indexed, int width, int height)
{
  const byte *palette = V_GetPlaypal();
  const byte *gamma = GammaTable();
  unsigned char *rgba = malloc(width * height * 4);
  int k;

  if (!rgba)
    return NULL;

  for (k = 0; k < width * height; ++k)
  {
    const int index = indexed[k * 2];

    rgba[k * 4 + 0] = gamma[palette[index * 3 + 0]];
    rgba[k * 4 + 1] = gamma[palette[index * 3 + 1]];
    rgba[k * 4 + 2] = gamma[palette[index * 3 + 2]];
    rgba[k * 4 + 3] = indexed[k * 2 + 1];
  }

  return rgba;
}

// Doom's other palettes mix each color toward one (red for pain, gold for
// items, green for the radiation suit): palette = normal * (1 - f) + tint * f.
// f and the tint, found from the PLAYPAL itself (so mods' palettes work).
typedef struct
{
  float red, green, blue, amount;
} tint_t;

static tint_t FindTint(int palette)
{
  const byte *normal = V_GetPlaypal();
  const byte *other = normal + palette * PALETTE_SIZE;
  tint_t tint = {0, 0, 0, 0};
  float amount = 0, sums[3];
  int channels = 0, c, k;

  if (palette <= 0 || palette >= V_GetPlaypalCount())
    return tint;

  // How much each channel's range shrinks: 1 - f.
  for (c = 0; c < 3; ++c)
  {
    int low = 0, high = 0;

    for (k = 1; k < 256; ++k)
    {
      if (normal[k * 3 + c] < normal[low * 3 + c])
        low = k;
      if (normal[k * 3 + c] > normal[high * 3 + c])
        high = k;
    }

    if (normal[high * 3 + c] - normal[low * 3 + c] > 64)
    {
      amount += 1 - (float) (other[high * 3 + c] - other[low * 3 + c])
                  / (normal[high * 3 + c] - normal[low * 3 + c]);
      ++channels;
    }
  }

  if (!channels)
    return tint;

  amount /= channels;

  if (amount < 0.01f)
    return tint;

  if (amount > 1)
    amount = 1;

  // The tint: what is left of each color once its share is taken away.
  for (c = 0; c < 3; ++c)
  {
    sums[c] = 0;

    for (k = 0; k < 256; ++k)
      sums[c] += other[k * 3 + c] - normal[k * 3 + c] * (1 - amount);

    sums[c] /= 256 * amount * 255;
    sums[c] = sums[c] < 0 ? 0 : sums[c] > 1 ? 1 : sums[c];
  }

  tint.red = sums[0];
  tint.green = sums[1];
  tint.blue = sums[2];
  tint.amount = amount;
  return tint;
}

static void BeforeSwap(void)
{
  static int last_palette = -1;
  static tint_t tint;

  if (current_palette != last_palette)
  {
    tint = FindTint(current_palette);
    last_palette = current_palette;
  }

  if (tint.amount > 0)
    pglBlendScreen(tint.red, tint.green, tint.blue, tint.amount);
}

// The bottom screen's palette: the current one, with gamma.
static const SDL_Color *BottomPalette(dboolean *changed)
{
  static SDL_Color colors[256];
  static int last_palette = -1, last_gamma = -1;
  const byte *palette, *gamma;
  int k;

  *changed = current_palette != last_palette || usegamma != last_gamma;

  if (!*changed)
    return colors;

  last_palette = current_palette;
  last_gamma = usegamma;
  palette = V_GetPlaypal();
  if (current_palette > 0 && current_palette < V_GetPlaypalCount())
    palette += current_palette * PALETTE_SIZE;
  gamma = GammaTable();

  for (k = 0; k < 256; ++k)
  {
    colors[k].r = gamma[palette[k * 3 + 0]];
    colors[k].g = gamma[palette[k * 3 + 1]];
    colors[k].b = gamma[palette[k * 3 + 2]];
    colors[k].a = 255;
  }

  return colors;
}

// ==========================================================================
// SDL

void *N3DS_GLCreateContext(void)
{
  if (!pglCreateContext())
    return NULL;

  pglSetBeforeSwap(BeforeSwap);
  return (void *) 1;
}

void N3DS_GLSwap(void)
{
  const SDL_Color *palette;
  dboolean changed;
  int work_us, gpu_us;

  pglSwapBuffers();
  pglFrameTimes(&work_us, &gpu_us);
  palette = BottomPalette(&changed);
  N3DS_PresentBottom(palette, changed, work_us, gpu_us);

  {
    static int frames;

    if (++frames <= 3)
      printf("N3DS_GLSwap: frame %d on the screens\n", frames);
  }

  // Every 10 seconds, per frame: the game's logic, the drawing (without
  // waiting for the GPU), of it pgl's part, the wait, and pgl's counts.
  {
    static int frames;
    static unsigned long long start;
    const unsigned long long now = N3DS_ProfileNow();

    ++frames;

    if (!start)
    {
      start = now;
    }
    else if (now - start > 10ULL * 268111856ULL)
    {
      double ms[N3DS_PROFILE_COUNT];
      int calls[N3DS_PROFILE_COUNT];
      pgl_stats_t stats;

      N3DS_ProfileTake(ms, calls);
      pglTakeStats(&stats);

      {
        extern unsigned int __ctru_heap_size;
        const struct mallinfo info = mallinfo();

        printf("memory: heap %u of %u KiB used, %u KiB free for the GPU\n",
               (unsigned) (info.uordblks / 1024), __ctru_heap_size / 1024,
               (unsigned) (N3DS_LinearFree() / 1024));
      }

      printf("perf: %d frames; per frame: logic %.1f ms (%d tics), drawing %.1f ms "
             "(pgl %.1f), waiting %.1f ms, uploads %.1f ms (%d); "
             "draws %d, binds %d, light changes %d, combiner changes %d\n",
             frames, ms[N3DS_PROFILE_TICS] / frames, calls[N3DS_PROFILE_TICS],
             (ms[N3DS_PROFILE_DISPLAY] - stats.wait_ms) / frames, stats.draw_ms / frames,
             stats.wait_ms / frames, stats.upload_ms / frames, stats.uploads,
             stats.draws / frames, stats.binds / frames, stats.fog_changes / frames,
             stats.texenv_changes / frames);
      frames = 0;
      start = now;
    }
  }
}

// ==========================================================================
// The shaders (gl_shader.c elsewhere)

// The light: DSDA-Doom's gls_main shader takes Doom's light index from the
// light level and the window depth (indexLight), then that row of the
// COLORMAP, where row i is about (32 - i) / 32 as bright. One fog table per
// half light level step.
#define LIGHT_STEPS 127

static pgl_fog_table_t *fog_tables[LIGHT_STEPS];

static pgl_fog_table_t *FogTable(float lightlevel)
{
  int step = (int) (lightlevel * (LIGHT_STEPS - 1) + 0.5f);

  step = step < 0 ? 0 : step >= LIGHT_STEPS ? LIGHT_STEPS - 1 : step;

  if (!fog_tables[step])
  {
    const float L = 63.0f * step / (LIGHT_STEPS - 1);
    const float min_index = L > 36 ? 0 : 36 - L > 31 ? 31 : 36 - L;
    float brightness[129];
    int k;

    for (k = 0; k <= 128; ++k)
    {
      const float z = k / 128.0f;
      float index = 59.0f + 200.0f - L - 200.0f / (z > 0.0001f ? z : 0.0001f);

      index = index < min_index ? min_index : index > 31 ? 31 : index;
      brightness[k] = (32 - index) / 32;
    }

    fog_tables[step] = pglNewFogTable(brightness);
  }

  return fog_tables[step];
}

typedef struct
{
  int program;
  float lightlevel;
} shader_frame_t;

#define MAX_STACK 16

static shader_frame_t stack[MAX_STACK];
static int depth;

static void Activate(void)
{
  const shader_frame_t *frame = depth > 0 ? &stack[depth - 1] : NULL;

  if (!frame)
  {
    pglProgram(PGL_PROGRAM_FIXED);
    return;
  }

  pglProgram(frame->program);

  if (frame->program == PGL_PROGRAM_LIGHT)
    pglFogTable(FogTable(frame->lightlevel));
}

static void Push(int program)
{
  if (depth == MAX_STACK)
    I_Error("ShaderPush: Max shader stack depth exceeded");

  stack[depth].program = program;
  stack[depth].lightlevel = 1; // full bright until said otherwise
  ++depth;
  Activate();
}

static void Pop(void)
{
  if (depth == 0)
    I_Error("ShaderPop: Pop of empty shader stack");

  --depth;
  Activate();
}

void glsl_Init(void)
{
  lprintf(LO_INFO, "glsl_Init: no shaders on the 3DS: fog for the light\n");
}

void glsl_SetTextureDims(int unit, unsigned int width, unsigned int height) {}

void glsl_PushNullShader(void) { Push(PGL_PROGRAM_FIXED); }
void glsl_PopNullShader(void) { Pop(); }
void glsl_PushMainShader(void) { Push(PGL_PROGRAM_LIGHT); }
void glsl_PopMainShader(void) { Pop(); }
void glsl_PushFuzzShader(int tic, int sprite, float ratio) { Push(PGL_PROGRAM_SHADOW); }
void glsl_PopFuzzShader(void) { Pop(); }

void glsl_SetLightLevel(float lightlevel)
{
  if (depth > 0 && stack[depth - 1].program == PGL_PROGRAM_LIGHT)
  {
    stack[depth - 1].lightlevel = lightlevel;
    pglFogTable(FogTable(lightlevel));
  }
}
