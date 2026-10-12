//
// [3DS] pgl: OpenGL 1.x on the 3DS GPU through citro3d. See pgl.h.
//
// How a frame goes:
// - The gl* calls only record: vertices go into a buffer in linear memory
//   (the memory the GPU reads), states into variables.
// - At each draw (glEnd, glDrawArrays), the states that changed become
//   citro3d calls, and the draw a PICA200 command. citro3d keeps the
//   commands in a list.
// - At the swap (SDL_GL_SwapWindow), the list goes to the GPU in one go,
//   and the image to the top screen when the GPU is done.
//
// Coordinates: the top screen is 400x240 but mounted sideways, so the
// image is drawn on a 240x400 target. Every projection gets a rotation
// (and the PICA200's depth range, -1 to 0) on top; viewports and scissor
// boxes are rotated to match.
//

#include <3ds.h>
#include <citro3d.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/glext.h>

#include "pgl.h"
#include "pgl_shader_shbin.h"

#define TOP_WIDTH 400
#define TOP_HEIGHT 240

#define COMMAND_BUFFER_SIZE (1024 * 1024)
#define VERTEX_BUFFER_SIZE (2 * 1024 * 1024)
#define MAX_TEXTURE_SIZE 1024
#define MIN_TEXTURE_SIZE 8
#define TEXTURE_UNITS 32 // states only: the PICA200 draws with unit 0

#define MODELVIEW_STACK 32
#define PROJECTION_STACK 4
#define TEXTURE_STACK 4

// Command words a draw may take at most, state changes included.
#define DRAW_COMMANDS 512

typedef struct
{
  float x, y, z;
  float s, t;
  u8 r, g, b, a;
} vertex_t;

#define MAX_VERTICES (VERTEX_BUFFER_SIZE / sizeof(vertex_t))

typedef struct
{
  bool used;          // the name was given out
  bool has_data;      // tex is allocated
  C3D_Tex tex;
  int width, height;  // as OpenGL sees it (tex is a power of two)
  GLenum min_filter, mag_filter, wrap_s, wrap_t;
  u32 frame;          // the last frame it was drawn in
} texture_t;

struct pgl_fog_table_s
{
  C3D_FogLut lut;
};

// Matrices are OpenGL's: column after column.
typedef float matrix_t[16];

typedef struct
{
  matrix_t stack[MODELVIEW_STACK];
  int depth, size;
} matrix_stack_t;

static bool initialized;
static C3D_RenderTarget *target;
static DVLB_s *shader_dvlb;
static shaderProgram_s shader;
static int u_mvp, u_modelview, u_texgen, u_texmtx;

static bool in_frame;
static u32 frame_number = 1;
static u64 wait_ticks;      // waiting for the GPU and the screen, this frame
static u64 last_swap;
static int work_us, gpu_us; // the last frame's
static void (*before_swap)(void);

static vertex_t *vertices;  // in linear memory
static int vertex_count;    // used this frame
static bool vertices_full;

// Freed when the GPU is done with the frames that may use them.
static void **pending_free;
static int pending_count, pending_size;

static GLenum error = GL_NO_ERROR;

// Immediate mode
static GLenum begin_mode;
static bool in_begin;
static int begin_first;
static float current_s, current_t;
static u8 current_color[4] = {255, 255, 255, 255};

// Matrices
static matrix_stack_t matrices[3]; // modelview, projection, texture
static int matrix_mode;            // index in matrices
static bool mvp_dirty = true, texmtx_dirty = true, texgen_dirty = true;

// Textures
static texture_t *textures;
static int texture_count;
static int active_texture;
static GLuint bound_texture[TEXTURE_UNITS];
static bool texture_2d[TEXTURE_UNITS];
static bool texture_dirty = true;
static int unpack_alignment = 4, pack_alignment = 4;

// glTexEnv (unit 0)
static GLenum env_mode = GL_MODULATE;
static GLenum combine_rgb = GL_MODULATE, combine_alpha = GL_MODULATE;
static GLenum source_rgb[3] = {GL_TEXTURE, GL_PREVIOUS, GL_CONSTANT};
static GLenum source_alpha[3] = {GL_TEXTURE, GL_PREVIOUS, GL_CONSTANT};
static GLenum operand_rgb[3] = {GL_SRC_COLOR, GL_SRC_COLOR, GL_SRC_ALPHA};
static GLenum operand_alpha[3] = {GL_SRC_ALPHA, GL_SRC_ALPHA, GL_SRC_ALPHA};
static u32 env_color;
static bool texenv_dirty = true;

// glTexGen
static bool texgen_on[4];  // S, T, R, Q
static float texgen_plane[4][4] = {
  {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
};

// Fragment states
static bool blend_on, alpha_test_on, depth_test_on, stencil_test_on;
static bool cull_on, scissor_on, polygon_offset_on;
static GLenum blend_src = GL_ONE, blend_dst = GL_ZERO;
static GLenum alpha_func = GL_ALWAYS;
static float alpha_ref;
static GLenum depth_func = GL_LESS;
static bool depth_mask = true;
static bool color_mask[4] = {true, true, true, true};
static GLenum stencil_func = GL_ALWAYS;
static int stencil_ref;
static u32 stencil_value_mask = 0xFF, stencil_write_mask = 0xFF;
static GLenum stencil_fail = GL_KEEP, stencil_zfail = GL_KEEP, stencil_zpass = GL_KEEP;
static GLenum cull_mode = GL_BACK, front_face = GL_CCW;
static float polygon_offset_units;
static GLenum shade_model = GL_SMOOTH;
static bool fragment_dirty = true;

// Viewport and scissor box, as OpenGL has them (origin at the bottom left).
static int viewport[4] = {0, 0, TOP_WIDTH, TOP_HEIGHT};
static int scissor_box[4] = {0, 0, TOP_WIDTH, TOP_HEIGHT};
static bool viewport_dirty = true;

// glClear
static float clear_color[4];
static float clear_depth = 1;
static int clear_stencil;

// pgl's own
static int program = PGL_PROGRAM_FIXED;
static pgl_fog_table_t *fog_table;
static bool program_dirty = true;

// Client arrays
typedef struct
{
  bool enabled;
  int size;
  GLenum type;
  int stride;
  const void *pointer;
} client_array_t;

static client_array_t vertex_array, color_array, texcoord_array[TEXTURE_UNITS];
static int client_active_texture;

static void SetError(GLenum e)
{
  if (error == GL_NO_ERROR)
    error = e;
}

// ==========================================================================
// Matrices

static void MatrixIdentity(float *m)
{
  memset(m, 0, sizeof(matrix_t));
  m[0] = m[5] = m[10] = m[15] = 1;
}

// out = a * b (out may not be a or b)
static void MatrixMultiply(float *out, const float *a, const float *b)
{
  int c, r;

  for (c = 0; c < 4; ++c)
    for (r = 0; r < 4; ++r)
      out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1]
                     + a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
}

static float *CurrentMatrix(void)
{
  matrix_stack_t *s = &matrices[matrix_mode];

  return s->stack[s->depth];
}

static void MatrixChanged(void)
{
  if (matrix_mode == 2)
    texmtx_dirty = true;
  else
    mvp_dirty = true;

  if (matrix_mode == 0)
    texgen_dirty = true;
}

// The current matrix times m.
static void MultiplyCurrent(const float *m)
{
  matrix_t result;

  MatrixMultiply(result, CurrentMatrix(), m);
  memcpy(CurrentMatrix(), result, sizeof(matrix_t));
  MatrixChanged();
}

// Into a citro3d uniform: one row per register.
static void UploadMatrix(int location, const float *m)
{
  C3D_Mtx mtx;
  int r, c;

  for (r = 0; r < 4; ++r)
    for (c = 0; c < 4; ++c)
      mtx.r[r].c[3 - c] = m[c * 4 + r];

  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, location, &mtx);
}

// The rotation of the top screen and the PICA200's depth range (-1 to 0):
// {{0,1,0,0},{-1,0,0,0},{0,0,0.5,-0.5},{0,0,0,1}}, as citro3d's
// Mtx_OrthoTilt and Mtx_PerspTilt put on their projections.
static const matrix_t screen_fix = {
  0, -1, 0, 0,
  1, 0, 0, 0,
  0, 0, 0.5f, 0,
  0, 0, -0.5f, 1,
};

static void UploadMatrices(void)
{
  if (mvp_dirty)
  {
    matrix_t projected, mvp;

    MatrixMultiply(projected, matrices[1].stack[matrices[1].depth],
                   matrices[0].stack[matrices[0].depth]);
    MatrixMultiply(mvp, screen_fix, projected);
    UploadMatrix(u_mvp, mvp);
    mvp_dirty = false;
  }

  if (texmtx_dirty)
  {
    UploadMatrix(u_texmtx, matrices[2].stack[matrices[2].depth]);
    texmtx_dirty = false;
  }

  if (texgen_dirty)
  {
    bool on = texgen_on[0] && texgen_on[1];

    C3D_BoolUnifSet(GPU_VERTEX_SHADER, 0, on);

    if (on)
    {
      matrix_t planes;
      int k;

      // One plane per row; Q is w (1) unless generated.
      for (k = 0; k < 4; ++k)
      {
        static const float w[4] = {0, 0, 0, 1};
        const float *plane = texgen_on[k] ? texgen_plane[k] : (k == 3 ? w : texgen_plane[2]);

        planes[0 * 4 + k] = plane[0];
        planes[1 * 4 + k] = plane[1];
        planes[2 * 4 + k] = plane[2];
        planes[3 * 4 + k] = plane[3];
      }

      UploadMatrix(u_texgen, planes);
      UploadMatrix(u_modelview, matrices[0].stack[matrices[0].depth]);
    }

    texgen_dirty = false;
  }
}

// ==========================================================================
// Frames

static void FreePending(void)
{
  int k;

  for (k = 0; k < pending_count; ++k)
    linearFree(pending_free[k]);

  pending_count = 0;
}

static void FreeLater(void *data)
{
  if (!data)
    return;

  if (pending_count == pending_size)
  {
    pending_size = pending_size ? pending_size * 2 : 64;
    pending_free = realloc(pending_free, pending_size * sizeof(*pending_free));
  }

  pending_free[pending_count++] = data;
}

static void SetAttributes(void)
{
  C3D_AttrInfo *attributes = C3D_GetAttrInfo();
  C3D_BufInfo *buffers = C3D_GetBufInfo();

  AttrInfo_Init(attributes);
  AttrInfo_AddLoader(attributes, 0, GPU_FLOAT, 3);         // x, y, z
  AttrInfo_AddLoader(attributes, 1, GPU_FLOAT, 2);         // s, t
  AttrInfo_AddLoader(attributes, 2, GPU_UNSIGNED_BYTE, 4); // r, g, b, a

  BufInfo_Init(buffers);
  BufInfo_Add(buffers, vertices, sizeof(vertex_t), 3, 0x210);
}

static void DirtyAll(void)
{
  mvp_dirty = texmtx_dirty = texgen_dirty = true;
  texture_dirty = texenv_dirty = fragment_dirty = true;
  viewport_dirty = program_dirty = true;
}

static void BeginFrame(void)
{
  u64 start;

  if (in_frame)
    return;

  start = svcGetSystemTick();
  C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
  wait_ticks += svcGetSystemTick() - start;
  C3D_FrameDrawOn(target);
  in_frame = true;

  // The GPU is done with everything before: its memory is free.
  FreePending();
  vertex_count = 0;
  vertices_full = false;

  C3D_BindProgram(&shader);
  SetAttributes();
  DirtyAll();
}

// Waits for the GPU to draw what is in the list so far (to read the
// image), and goes on with the same frame. Not shown on the screen.
static void FinishDrawing(void)
{
  if (in_frame)
  {
    target->used = false; // no transfer to the screen
    C3D_FrameEnd(0);
  }

  C3D_FrameBegin(0);
  C3D_FrameDrawOn(target);

  if (!in_frame)
  {
    in_frame = true;
    FreePending();
    vertex_count = 0;
    vertices_full = false;
  }

  C3D_BindProgram(&shader);
  SetAttributes();
  DirtyAll();
}

// ==========================================================================
// Rotating rectangles to the 240x400 target

// x, y, w, h on the 400x240 screen (OpenGL: from the bottom left) to the
// target's x, y, w, h.
static void RotateRect(const int *rect, int *out)
{
  out[0] = rect[1];
  out[1] = TOP_WIDTH - (rect[0] + rect[2]);
  out[2] = rect[3];
  out[3] = rect[2];
}

static void ClipRect(int *rect, int width, int height)
{
  int x0 = rect[0] < 0 ? 0 : rect[0];
  int y0 = rect[1] < 0 ? 0 : rect[1];
  int x1 = rect[0] + rect[2] > width ? width : rect[0] + rect[2];
  int y1 = rect[1] + rect[3] > height ? height : rect[1] + rect[3];

  rect[0] = x0;
  rect[1] = y0;
  rect[2] = x1 > x0 ? x1 - x0 : 0;
  rect[3] = y1 > y0 ? y1 - y0 : 0;
}

static void ApplyViewport(void)
{
  int r[4];

  RotateRect(viewport, r);
  C3D_SetViewport(r[0], r[1], r[2], r[3]);

  // C3D_SetViewport turns the scissor test off.
  if (scissor_on)
  {
    RotateRect(scissor_box, r);
    ClipRect(r, TOP_HEIGHT, TOP_WIDTH);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, r[0], r[1], r[0] + r[2], r[1] + r[3]);
  }
}

// ==========================================================================
// States to citro3d

static GPU_TESTFUNC TestFunc(GLenum func)
{
  switch (func)
  {
    case GL_NEVER: return GPU_NEVER;
    case GL_LESS: return GPU_LESS;
    case GL_EQUAL: return GPU_EQUAL;
    case GL_LEQUAL: return GPU_LEQUAL;
    case GL_GREATER: return GPU_GREATER;
    case GL_NOTEQUAL: return GPU_NOTEQUAL;
    case GL_GEQUAL: return GPU_GEQUAL;
    default: return GPU_ALWAYS;
  }
}

// The PICA200's depth runs the other way (near is 1, far is 0).
static GPU_TESTFUNC DepthFunc(GLenum func)
{
  switch (func)
  {
    case GL_LESS: return GPU_GREATER;
    case GL_LEQUAL: return GPU_GEQUAL;
    case GL_GREATER: return GPU_LESS;
    case GL_GEQUAL: return GPU_LEQUAL;
    default: return TestFunc(func);
  }
}

static GPU_BLENDFACTOR BlendFactor(GLenum factor)
{
  switch (factor)
  {
    case GL_ZERO: return GPU_ZERO;
    case GL_SRC_COLOR: return GPU_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return GPU_ONE_MINUS_SRC_COLOR;
    case GL_DST_COLOR: return GPU_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return GPU_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA: return GPU_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return GPU_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return GPU_ONE_MINUS_DST_ALPHA;
    case GL_CONSTANT_COLOR: return GPU_CONSTANT_COLOR;
    case GL_ONE_MINUS_CONSTANT_COLOR: return GPU_ONE_MINUS_CONSTANT_COLOR;
    case GL_CONSTANT_ALPHA: return GPU_CONSTANT_ALPHA;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return GPU_ONE_MINUS_CONSTANT_ALPHA;
    case GL_SRC_ALPHA_SATURATE: return GPU_SRC_ALPHA_SATURATE;
    default: return GPU_ONE;
  }
}

static GPU_STENCILOP StencilOp(GLenum op)
{
  switch (op)
  {
    case GL_ZERO: return GPU_STENCIL_ZERO;
    case GL_REPLACE: return GPU_STENCIL_REPLACE;
    case GL_INCR: return GPU_STENCIL_INCR;
    case GL_DECR: return GPU_STENCIL_DECR;
    case GL_INVERT: return GPU_STENCIL_INVERT;
    case GL_INCR_WRAP: return GPU_STENCIL_INCR_WRAP;
    case GL_DECR_WRAP: return GPU_STENCIL_DECR_WRAP;
    default: return GPU_STENCIL_KEEP;
  }
}

static void ApplyFragment(void)
{
  int write = 0;

  if (color_mask[0]) write |= GPU_WRITE_RED;
  if (color_mask[1]) write |= GPU_WRITE_GREEN;
  if (color_mask[2]) write |= GPU_WRITE_BLUE;
  if (color_mask[3]) write |= GPU_WRITE_ALPHA;

  // Without the test, OpenGL writes no depth either.
  if (depth_test_on)
    C3D_DepthTest(true, DepthFunc(depth_func), write | (depth_mask ? GPU_WRITE_DEPTH : 0));
  else
    C3D_DepthTest(false, GPU_ALWAYS, write);

  if (blend_on)
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, BlendFactor(blend_src), BlendFactor(blend_dst),
                   BlendFactor(blend_src), BlendFactor(blend_dst));
  else
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

  if (alpha_test_on)
  {
    int ref = (int) (alpha_ref * 255 + 0.5f);

    C3D_AlphaTest(true, TestFunc(alpha_func), ref < 0 ? 0 : ref > 255 ? 255 : ref);
  }
  else
  {
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
  }

  if (stencil_test_on)
  {
    C3D_StencilTest(true, TestFunc(stencil_func), stencil_ref,
                    stencil_value_mask, stencil_write_mask);
    C3D_StencilOp(StencilOp(stencil_fail), StencilOp(stencil_zfail), StencilOp(stencil_zpass));
  }
  else
  {
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
  }

  // Front faces are counterclockwise (glFrontFace(GL_CCW)); the rotation
  // keeps them so.
  if (!cull_on || cull_mode == GL_FRONT_AND_BACK)
    C3D_CullFace(GPU_CULL_NONE);
  else if ((cull_mode == GL_BACK) == (front_face == GL_CCW))
    C3D_CullFace(GPU_CULL_BACK_CCW);
  else
    C3D_CullFace(GPU_CULL_FRONT_CCW);

  // glPolygonOffset: units of the 24-bit depth, toward the far side.
  C3D_DepthMap(true, -1.0f,
               polygon_offset_on ? -polygon_offset_units / 16777215.0f : 0.0f);
}

// A texture unit's binding, or NULL.
static texture_t *BoundTexture(int unit)
{
  GLuint name = bound_texture[unit];

  if (name == 0 || name >= (GLuint) texture_count || !textures[name].has_data)
    return NULL;

  return &textures[name];
}

static texture_t *DrawTexture(void)
{
  return texture_2d[0] ? BoundTexture(0) : NULL;
}

static void ApplyTexture(void)
{
  texture_t *t = DrawTexture();

  if (!t)
  {
    C3D_TexBind(0, NULL);
    return;
  }

  t->frame = frame_number;

  // glTexGen with Q: the coordinates are divided by q (texcoord0w).
  if (texgen_on[3] && texgen_on[0])
  {
    static C3D_Tex projected;

    projected = t->tex;
    projected.param = (projected.param & ~(7 << 28)) | ((u32) GPU_TEX_PROJECTION << 28);
    C3D_TexBind(0, &projected);
  }
  else
  {
    C3D_TexBind(0, &t->tex);
  }
}

static GPU_TEVSRC EnvSource(GLenum source)
{
  switch (source)
  {
    case GL_TEXTURE:
    case GL_TEXTURE0:
      return GPU_TEXTURE0;
    case GL_CONSTANT:
      return GPU_CONSTANT;
    default: // GL_PRIMARY_COLOR, GL_PREVIOUS (the color, on unit 0)
      return GPU_PRIMARY_COLOR;
  }
}

static GPU_TEVOP_RGB EnvOperandRgb(GLenum operand)
{
  switch (operand)
  {
    case GL_ONE_MINUS_SRC_COLOR: return GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR;
    case GL_SRC_ALPHA: return GPU_TEVOP_RGB_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA;
    default: return GPU_TEVOP_RGB_SRC_COLOR;
  }
}

static GPU_TEVOP_A EnvOperandAlpha(GLenum operand)
{
  return operand == GL_ONE_MINUS_SRC_ALPHA ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA
                                           : GPU_TEVOP_A_SRC_ALPHA;
}

static GPU_COMBINEFUNC EnvFunc(GLenum func)
{
  switch (func)
  {
    case GL_REPLACE: return GPU_REPLACE;
    case GL_ADD: return GPU_ADD;
    case GL_ADD_SIGNED: return GPU_ADD_SIGNED;
    case GL_INTERPOLATE: return GPU_INTERPOLATE;
    case GL_SUBTRACT: return GPU_SUBTRACT;
    case GL_DOT3_RGB: return GPU_DOT3_RGB;
    case GL_DOT3_RGBA: return GPU_DOT3_RGBA;
    default: return GPU_MODULATE;
  }
}

// The first color combiner stage does what glTexEnv (or the program)
// says; the others pass it on.
static void ApplyTexEnv(void)
{
  C3D_TexEnv env;
  bool textured = DrawTexture() != NULL;

  C3D_TexEnvInit(&env);
  C3D_TexEnvColor(&env, env_color);

  if (program == PGL_PROGRAM_SHADOW)
  {
    C3D_TexEnvSrc(&env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(&env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvColor(&env, 0x80FFFFFF);

    if (textured)
    {
      C3D_TexEnvSrc(&env, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
      C3D_TexEnvFunc(&env, C3D_Alpha, GPU_MODULATE);
    }
    else
    {
      C3D_TexEnvSrc(&env, C3D_Alpha, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
      C3D_TexEnvFunc(&env, C3D_Alpha, GPU_REPLACE);
    }
  }
  else if (!textured)
  {
    // No texture: OpenGL skips the texture environment.
    C3D_TexEnvSrc(&env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(&env, C3D_Both, GPU_REPLACE);
  }
  else if (program == PGL_PROGRAM_LIGHT || env_mode == GL_MODULATE)
  {
    C3D_TexEnvSrc(&env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(&env, C3D_Both, GPU_MODULATE);
  }
  else if (env_mode == GL_REPLACE)
  {
    C3D_TexEnvSrc(&env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(&env, C3D_Both, GPU_REPLACE);
  }
  else if (env_mode == GL_DECAL)
  {
    C3D_TexEnvSrc(&env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
    C3D_TexEnvOpRgb(&env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(&env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(&env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(&env, C3D_Alpha, GPU_REPLACE);
  }
  else if (env_mode == GL_ADD)
  {
    C3D_TexEnvSrc(&env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(&env, C3D_RGB, GPU_ADD);
    C3D_TexEnvFunc(&env, C3D_Alpha, GPU_MODULATE);
  }
  else if (env_mode == GL_BLEND)
  {
    C3D_TexEnvSrc(&env, C3D_RGB, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
    C3D_TexEnvFunc(&env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(&env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(&env, C3D_Alpha, GPU_MODULATE);
  }
  else // GL_COMBINE
  {
    C3D_TexEnvSrc(&env, C3D_RGB, EnvSource(source_rgb[0]), EnvSource(source_rgb[1]),
                  EnvSource(source_rgb[2]));
    C3D_TexEnvSrc(&env, C3D_Alpha, EnvSource(source_alpha[0]), EnvSource(source_alpha[1]),
                  EnvSource(source_alpha[2]));
    C3D_TexEnvOpRgb(&env, EnvOperandRgb(operand_rgb[0]), EnvOperandRgb(operand_rgb[1]),
                    EnvOperandRgb(operand_rgb[2]));
    C3D_TexEnvOpAlpha(&env, EnvOperandAlpha(operand_alpha[0]), EnvOperandAlpha(operand_alpha[1]),
                      EnvOperandAlpha(operand_alpha[2]));
    C3D_TexEnvFunc(&env, C3D_RGB, EnvFunc(combine_rgb));
    C3D_TexEnvFunc(&env, C3D_Alpha, EnvFunc(combine_alpha));
  }

  C3D_SetTexEnv(0, &env);
}

static void ApplyProgram(void)
{
  if (program == PGL_PROGRAM_LIGHT && fog_table)
  {
    C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
    C3D_FogColor(0);
    C3D_FogLutBind(&fog_table->lut);
  }
  else
  {
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_FogLutBind(NULL);
  }
}

static void ApplyStates(void)
{
  UploadMatrices();

  if (texture_dirty)
  {
    ApplyTexture();
    texture_dirty = false;
    texenv_dirty = true; // with or without a texture
  }

  if (program_dirty)
  {
    ApplyProgram();
    program_dirty = false;
    texenv_dirty = true;
  }

  if (texenv_dirty)
  {
    ApplyTexEnv();
    texenv_dirty = false;
  }

  if (fragment_dirty)
  {
    ApplyFragment();
    fragment_dirty = false;
  }

  if (viewport_dirty)
  {
    ApplyViewport();
    viewport_dirty = false;
  }
}

// ==========================================================================
// Drawing

static int frame_draws, frame_skipped;

static bool RoomForCommands(void)
{
  u32 size, offset;

  GPUCMD_GetBuffer(NULL, &size, &offset);

  if (size - offset < DRAW_COMMANDS)
  {
    if (frame_skipped++ == 0)
      printf("pgl: the command list is full (%d draws)\n", frame_draws);
    return false;
  }

  return true;
}

// Room for count more vertices in this frame's buffer.
static vertex_t *ReserveVertices(int count)
{
  if (vertex_count + count > (int) MAX_VERTICES)
  {
    if (!vertices_full)
      printf("pgl: the vertex buffer is full\n");
    vertices_full = true;
    return NULL;
  }

  return vertices + vertex_count;
}

static void DrawVertices(GPU_Primitive_t primitive, int first, int count)
{
  if (count <= 0 || !RoomForCommands())
    return;

  ApplyStates();
  C3D_DrawArrays(primitive, first, count);
  ++frame_draws;
}

// Lines and points are quads on the PICA200: one pixel wide, made here
// from the vertices projected to the screen, and drawn without matrices.
static void DrawLinesAsQuads(const vertex_t *source, int count, GLenum mode)
{
  matrix_t projected, mvp;
  int segments, k, r[4];
  vertex_t *out;

  MatrixMultiply(projected, matrices[1].stack[matrices[1].depth],
                 matrices[0].stack[matrices[0].depth]);
  MatrixMultiply(mvp, screen_fix, projected);

  if (mode == GL_LINES)
    segments = count / 2;
  else if (mode == GL_LINE_LOOP)
    segments = count;
  else if (mode == GL_LINE_STRIP)
    segments = count - 1;
  else // GL_POINTS: a segment from each point to itself
    segments = count;

  if (segments <= 0)
    return;

  out = ReserveVertices(segments * 6);

  if (!out)
    return;

  RotateRect(viewport, r);

  for (k = 0; k < segments; ++k)
  {
    const vertex_t *a, *b;
    float p[2][4], dx, dy, length, nx, ny;
    int i, j;

    if (mode == GL_LINES)
      a = &source[k * 2], b = &source[k * 2 + 1];
    else if (mode == GL_POINTS)
      a = b = &source[k];
    else
      a = &source[k], b = &source[(k + 1) % count];

    for (i = 0; i < 2; ++i)
    {
      const vertex_t *v = i ? b : a;

      for (j = 0; j < 4; ++j)
        p[i][j] = mvp[0 * 4 + j] * v->x + mvp[1 * 4 + j] * v->y
                + mvp[2 * 4 + j] * v->z + mvp[3 * 4 + j];

      if (p[i][3] <= 0)
        break;

      p[i][0] /= p[i][3];
      p[i][1] /= p[i][3];
      p[i][2] /= p[i][3];
    }

    if (i < 2)
    {
      memset(out, 0, 6 * sizeof(*out)); // behind the eye: nothing
      out += 6;
      continue;
    }

    // Half a pixel to each side, in the target's pixels.
    dx = (p[1][0] - p[0][0]) * r[2] / 2;
    dy = (p[1][1] - p[0][1]) * r[3] / 2;
    length = sqrtf(dx * dx + dy * dy);

    if (length < 0.0001f)
    {
      dx = 1;
      dy = 0;
      length = 1;
    }

    nx = -dy / length * 0.5f * 2 / r[2];
    ny = dx / length * 0.5f * 2 / r[3];

    // Points and line ends: half a pixel longer.
    dx = dx / length * 0.5f * 2 / r[2];
    dy = dy / length * 0.5f * 2 / r[3];

    {
      const float corners[4][2] = {
        {p[0][0] - dx + nx, p[0][1] - dy + ny},
        {p[0][0] - dx - nx, p[0][1] - dy - ny},
        {p[1][0] + dx + nx, p[1][1] + dy + ny},
        {p[1][0] + dx - nx, p[1][1] + dy - ny},
      };
      static const int order[6] = {0, 1, 2, 2, 1, 3};

      for (i = 0; i < 6; ++i)
      {
        const int c = order[i];
        // Flat shading: the last vertex's color, as OpenGL does.
        const vertex_t *color = (shade_model == GL_FLAT || c >= 2) ? b : a;

        out[i] = *color;
        out[i].x = corners[c][0];
        out[i].y = corners[c][1];
        out[i].z = c < 2 ? p[0][2] : p[1][2];
      }
    }

    out += 6;
  }

  // Already projected: no matrices for these.
  {
    static const matrix_t identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const int first = vertex_count;

    vertex_count += segments * 6;
    UploadMatrices();
    UploadMatrix(u_mvp, identity);
    DrawVertices(GPU_TRIANGLES, first, segments * 6);
    mvp_dirty = true;
  }
}

// count vertices at vertices[first], in an OpenGL mode.
static void DrawMode(GLenum mode, int first, int count)
{
  vertex_t *source = vertices + first;

  switch (mode)
  {
    case GL_TRIANGLES:
      DrawVertices(GPU_TRIANGLES, first, count - count % 3);
      break;

    case GL_TRIANGLE_STRIP:
    case GL_QUAD_STRIP: // the same vertices, the same triangles
      DrawVertices(GPU_TRIANGLE_STRIP, first, count);
      break;

    case GL_TRIANGLE_FAN:
    case GL_POLYGON:
      DrawVertices(GPU_TRIANGLE_FAN, first, count);
      break;

    case GL_QUADS:
    {
      const int quads = count / 4;
      vertex_t *out = ReserveVertices(quads * 6);
      int k;

      if (!out)
        break;

      for (k = 0; k < quads; ++k)
      {
        const vertex_t *q = source + k * 4;

        out[0] = q[0]; out[1] = q[1]; out[2] = q[2];
        out[3] = q[0]; out[4] = q[2]; out[5] = q[3];
        out += 6;
      }

      first = vertex_count;
      vertex_count += quads * 6;
      DrawVertices(GPU_TRIANGLES, first, quads * 6);
      break;
    }

    case GL_LINES:
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
    case GL_POINTS:
      DrawLinesAsQuads(source, count, mode);
      break;

    default:
      SetError(GL_INVALID_ENUM);
      break;
  }
}

// ==========================================================================
// Context

bool pglCreateContext(void)
{
  int k;

  if (initialized)
    return true;

  if (!C3D_Init(COMMAND_BUFFER_SIZE))
  {
    printf("pgl: C3D_Init failed\n");
    return false;
  }

  target = C3D_RenderTargetCreate(TOP_HEIGHT, TOP_WIDTH, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);

  if (!target)
  {
    printf("pgl: no render target\n");
    C3D_Fini();
    return false;
  }

  C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT,
    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0)
    | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8)
    | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

  shader_dvlb = DVLB_ParseFile((u32 *) pgl_shader_shbin, pgl_shader_shbin_size);
  shaderProgramInit(&shader);
  shaderProgramSetVsh(&shader, &shader_dvlb->DVLE[0]);
  u_mvp = shaderInstanceGetUniformLocation(shader.vertexShader, "mvp");
  u_modelview = shaderInstanceGetUniformLocation(shader.vertexShader, "modelview");
  u_texgen = shaderInstanceGetUniformLocation(shader.vertexShader, "texgen");
  u_texmtx = shaderInstanceGetUniformLocation(shader.vertexShader, "texmtx");

  vertices = linearAlloc(VERTEX_BUFFER_SIZE);

  if (!vertices)
  {
    printf("pgl: no memory for the vertex buffer\n");
    return false;
  }

  for (k = 0; k < 3; ++k)
  {
    matrices[k].depth = 0;
    MatrixIdentity(matrices[k].stack[0]);
  }

  matrices[0].size = MODELVIEW_STACK;
  matrices[1].size = PROJECTION_STACK;
  matrices[2].size = TEXTURE_STACK;

  // The other combiner stages pass the first one's color on.
  for (k = 1; k < 6; ++k)
    C3D_TexEnvInit(C3D_GetTexEnv(k));

  initialized = true;
  DirtyAll();
  printf("pgl: OpenGL on citro3d ready\n");
  return true;
}

void pglDeleteContext(void)
{
  int k;

  if (!initialized)
    return;

  if (in_frame)
  {
    target->used = false;
    C3D_FrameEnd(0);
    in_frame = false;
  }

  C3D_Fini(); // waits for the GPU
  FreePending();

  for (k = 0; k < texture_count; ++k)
    if (textures[k].has_data)
      C3D_TexDelete(&textures[k].tex);

  free(textures);
  textures = NULL;
  texture_count = 0;

  linearFree(vertices);
  vertices = NULL;
  shaderProgramFree(&shader);
  DVLB_Free(shader_dvlb);
  target = NULL;
  initialized = false;
}

void pglSetBeforeSwap(void (*callback)(void))
{
  before_swap = callback;
}

void pglSwapBuffers(void)
{
  static u32 report_time;
  static int max_draws;
  static float max_usage;
  float usage;

  if (!initialized)
    return;

  BeginFrame();

  if (before_swap)
    before_swap();

  usage = C3D_GetCmdBufUsage();
  C3D_FrameEnd(0);
  in_frame = false;
  ++frame_number;

  {
    const u64 now = svcGetSystemTick();

    if (last_swap)
      work_us = (int) ((now - last_swap - wait_ticks) / CPU_TICKS_PER_USEC);
    gpu_us = (int) (C3D_GetDrawingTime() * 1000);
    last_swap = now;
    wait_ticks = 0;
  }

  // In the log, every 10 seconds: how full the lists get.
  if (frame_draws > max_draws)
    max_draws = frame_draws;
  if (usage > max_usage)
    max_usage = usage;

  if (osGetTime() - report_time > 10000)
  {
    if (report_time)
      printf("pgl: up to %d draws, %d%% of the command list, %d vertices\n",
             max_draws, (int) (max_usage * 100), vertex_count);
    report_time = osGetTime();
    max_draws = 0;
    max_usage = 0;
  }

  frame_draws = 0;
  frame_skipped = 0;
}

void pglFrameTimes(int *work, int *gpu)
{
  *work = work_us;
  *gpu = gpu_us;
}

void pglProgram(int new_program)
{
  if (program != new_program)
  {
    program = new_program;
    program_dirty = true;
  }
}

pgl_fog_table_t *pglNewFogTable(const float brightness[129])
{
  pgl_fog_table_t *table = malloc(sizeof(*table));
  float data[256];
  float values[129];
  int k;

  // The PICA200 looks fog up by its depth (1 near, 0 far): window depth
  // 1 - k / 128.
  for (k = 0; k <= 128; ++k)
    values[k] = brightness[128 - k];

  for (k = 0; k < 128; ++k)
  {
    data[k] = values[k];
    data[128 + k] = values[k + 1] - values[k];
  }

  FogLut_FromArray(&table->lut, data);
  return table;
}

void pglFogTable(pgl_fog_table_t *table)
{
  if (fog_table != table)
  {
    fog_table = table;

    if (program == PGL_PROGRAM_LIGHT)
      program_dirty = true;
  }
}

// ==========================================================================
// OpenGL: errors, strings, queries

GLenum glGetError(void)
{
  GLenum e = error;

  error = GL_NO_ERROR;
  return e;
}

const GLubyte *glGetString(GLenum name)
{
  switch (name)
  {
    case GL_VENDOR: return (const GLubyte *) "pgl (citro3d)";
    case GL_RENDERER: return (const GLubyte *) "PICA200";
    case GL_VERSION: return (const GLubyte *) "1.3 pgl";
    case GL_EXTENSIONS:
      return (const GLubyte *) "GL_ARB_multitexture GL_ARB_texture_non_power_of_two "
                               "GL_ARB_texture_env_combine GL_EXT_texture_env_combine";
    default:
      SetError(GL_INVALID_ENUM);
      return NULL;
  }
}

void glGetIntegerv(GLenum name, GLint *params)
{
  switch (name)
  {
    case GL_MAX_TEXTURE_SIZE: *params = MAX_TEXTURE_SIZE; break;
    case GL_MAX_TEXTURE_UNITS: *params = TEXTURE_UNITS; break;
    case GL_SHADE_MODEL: *params = shade_model; break;
    case GL_PACK_ALIGNMENT: *params = pack_alignment; break;
    case GL_UNPACK_ALIGNMENT: *params = unpack_alignment; break;
    case GL_BLEND_SRC: *params = blend_src; break;
    case GL_BLEND_DST: *params = blend_dst; break;
    case GL_VIEWPORT: memcpy(params, viewport, sizeof(viewport)); break;
    case GL_SCISSOR_BOX: memcpy(params, scissor_box, sizeof(scissor_box)); break;
    case GL_MATRIX_MODE:
      *params = matrix_mode == 0 ? GL_MODELVIEW : matrix_mode == 1 ? GL_PROJECTION : GL_TEXTURE;
      break;
    case GL_TEXTURE_BINDING_2D: *params = bound_texture[active_texture]; break;
    case GL_DEPTH_BITS: *params = 24; break;
    case GL_STENCIL_BITS: *params = 8; break;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS:
      *params = 8;
      break;
    default:
      *params = 0;
      SetError(GL_INVALID_ENUM);
      break;
  }
}

void glGetFloatv(GLenum name, GLfloat *params)
{
  switch (name)
  {
    case GL_MODELVIEW_MATRIX: memcpy(params, matrices[0].stack[matrices[0].depth], sizeof(matrix_t)); break;
    case GL_PROJECTION_MATRIX: memcpy(params, matrices[1].stack[matrices[1].depth], sizeof(matrix_t)); break;
    case GL_TEXTURE_MATRIX: memcpy(params, matrices[2].stack[matrices[2].depth], sizeof(matrix_t)); break;
    default:
    {
      GLint value;

      glGetIntegerv(name, &value);
      *params = (GLfloat) value;
      break;
    }
  }
}

void glHint(GLenum target_, GLenum mode) {}
void glFlush(void) {}
void glFinish(void) {}

void glPixelStorei(GLenum name, GLint param)
{
  if (name == GL_UNPACK_ALIGNMENT)
    unpack_alignment = param;
  else if (name == GL_PACK_ALIGNMENT)
    pack_alignment = param;
}

// ==========================================================================
// OpenGL: capabilities

static void SetCapability(GLenum cap, bool on)
{
  switch (cap)
  {
    case GL_TEXTURE_2D:
      if (texture_2d[active_texture] != on)
      {
        texture_2d[active_texture] = on;
        if (active_texture == 0)
          texture_dirty = true;
      }
      break;
    case GL_BLEND: blend_on = on; fragment_dirty = true; break;
    case GL_ALPHA_TEST: alpha_test_on = on; fragment_dirty = true; break;
    case GL_DEPTH_TEST: depth_test_on = on; fragment_dirty = true; break;
    case GL_STENCIL_TEST: stencil_test_on = on; fragment_dirty = true; break;
    case GL_CULL_FACE: cull_on = on; fragment_dirty = true; break;
    case GL_POLYGON_OFFSET_FILL: polygon_offset_on = on; fragment_dirty = true; break;
    case GL_SCISSOR_TEST: scissor_on = on; viewport_dirty = true; break;
    case GL_TEXTURE_GEN_S: texgen_on[0] = on; texgen_dirty = texture_dirty = true; break;
    case GL_TEXTURE_GEN_T: texgen_on[1] = on; texgen_dirty = texture_dirty = true; break;
    case GL_TEXTURE_GEN_R: texgen_on[2] = on; texgen_dirty = texture_dirty = true; break;
    case GL_TEXTURE_GEN_Q: texgen_on[3] = on; texgen_dirty = texture_dirty = true; break;
    default: break; // fog, multisampling, depth clamp, dithering...: nothing to do
  }
}

void glEnable(GLenum cap) { SetCapability(cap, true); }
void glDisable(GLenum cap) { SetCapability(cap, false); }

GLboolean glIsEnabled(GLenum cap)
{
  switch (cap)
  {
    case GL_TEXTURE_2D: return texture_2d[active_texture];
    case GL_BLEND: return blend_on;
    case GL_ALPHA_TEST: return alpha_test_on;
    case GL_DEPTH_TEST: return depth_test_on;
    case GL_STENCIL_TEST: return stencil_test_on;
    case GL_CULL_FACE: return cull_on;
    case GL_SCISSOR_TEST: return scissor_on;
    default: return GL_FALSE;
  }
}

static client_array_t *ClientArray(GLenum array)
{
  switch (array)
  {
    case GL_VERTEX_ARRAY: return &vertex_array;
    case GL_COLOR_ARRAY: return &color_array;
    case GL_TEXTURE_COORD_ARRAY: return &texcoord_array[client_active_texture];
    default: return NULL;
  }
}

void glEnableClientState(GLenum array)
{
  client_array_t *a = ClientArray(array);

  if (a)
    a->enabled = true;
}

void glDisableClientState(GLenum array)
{
  client_array_t *a = ClientArray(array);

  if (a)
    a->enabled = false;
}

void glActiveTexture(GLenum texture)
{
  int unit = texture - GL_TEXTURE0;

  if (unit < 0 || unit >= TEXTURE_UNITS)
  {
    SetError(GL_INVALID_ENUM);
    return;
  }

  active_texture = unit;
}

void glClientActiveTexture(GLenum texture)
{
  int unit = texture - GL_TEXTURE0;

  if (unit >= 0 && unit < TEXTURE_UNITS)
    client_active_texture = unit;
}

// ==========================================================================
// OpenGL: fragment states

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
  blend_src = sfactor;
  blend_dst = dfactor;
  fragment_dirty = true;
}

void glAlphaFunc(GLenum func, GLclampf ref)
{
  alpha_func = func;
  alpha_ref = ref;
  fragment_dirty = true;
}

void glDepthFunc(GLenum func)
{
  depth_func = func;
  fragment_dirty = true;
}

void glDepthMask(GLboolean flag)
{
  depth_mask = flag;
  fragment_dirty = true;
}

void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
  color_mask[0] = red;
  color_mask[1] = green;
  color_mask[2] = blue;
  color_mask[3] = alpha;
  fragment_dirty = true;
}

void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
  stencil_func = func;
  stencil_ref = ref;
  stencil_value_mask = mask & 0xFF;
  fragment_dirty = true;
}

void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
  stencil_fail = fail;
  stencil_zfail = zfail;
  stencil_zpass = zpass;
  fragment_dirty = true;
}

void glStencilMask(GLuint mask)
{
  stencil_write_mask = mask & 0xFF;
  fragment_dirty = true;
}

void glCullFace(GLenum mode)
{
  cull_mode = mode;
  fragment_dirty = true;
}

void glFrontFace(GLenum mode)
{
  front_face = mode;
  fragment_dirty = true;
}

void glPolygonOffset(GLfloat factor, GLfloat units)
{
  polygon_offset_units = units;
  fragment_dirty = true;
}

void glShadeModel(GLenum mode)
{
  shade_model = mode;
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
  viewport[0] = x;
  viewport[1] = y;
  viewport[2] = width;
  viewport[3] = height;
  viewport_dirty = true;
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
  scissor_box[0] = x;
  scissor_box[1] = y;
  scissor_box[2] = width;
  scissor_box[3] = height;
  viewport_dirty = true;
}

void glDepthRange(GLclampd near_val, GLclampd far_val) {}

// ==========================================================================
// OpenGL: glClear

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
  clear_color[0] = red;
  clear_color[1] = green;
  clear_color[2] = blue;
  clear_color[3] = alpha;
}

void glClearDepth(GLclampd depth)
{
  clear_depth = (float) depth;
}

void glClearStencil(GLint s)
{
  clear_stencil = s;
}

static u8 ColorByte(float value)
{
  return value <= 0 ? 0 : value >= 1 ? 255 : (u8) (value * 255 + 0.5f);
}

// A quad over the whole target, with its own states: for glClear (what
// the scissor box and masks leave out) and pglBlendScreen.
static void ScreenQuad(const float *color, float z, int write, bool blend, bool stencil,
                       bool scissor)
{
  static const matrix_t identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  vertex_t *v = ReserveVertices(4);
  int k, r[4];

  if (!v || !RoomForCommands())
    return;

  for (k = 0; k < 4; ++k)
  {
    v[k].x = (k & 1) ? 1 : -1;
    v[k].y = (k & 2) ? 1 : -1;
    v[k].z = z;
    v[k].s = v[k].t = 0;
    v[k].r = ColorByte(color[0]);
    v[k].g = ColorByte(color[1]);
    v[k].b = ColorByte(color[2]);
    v[k].a = ColorByte(color[3]);
  }

  UploadMatrices();
  UploadMatrix(u_mvp, identity);
  C3D_TexBind(0, NULL);
  {
    C3D_TexEnv env;

    C3D_TexEnvInit(&env);
    C3D_TexEnvSrc(&env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(&env, C3D_Both, GPU_REPLACE);
    C3D_SetTexEnv(0, &env);
  }
  C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
  C3D_DepthTest(true, GPU_ALWAYS, write);
  C3D_DepthMap(true, -1.0f, 0.0f);
  C3D_AlphaTest(false, GPU_ALWAYS, 0);
  C3D_CullFace(GPU_CULL_NONE);

  if (blend)
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
  else
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

  if (stencil)
  {
    C3D_StencilTest(true, GPU_ALWAYS, clear_stencil, 0xFF, stencil_write_mask);
    C3D_StencilOp(GPU_STENCIL_REPLACE, GPU_STENCIL_REPLACE, GPU_STENCIL_REPLACE);
  }
  else
  {
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
  }

  C3D_SetViewport(0, 0, TOP_HEIGHT, TOP_WIDTH);
  if (scissor)
  {
    RotateRect(scissor_box, r);
    ClipRect(r, TOP_HEIGHT, TOP_WIDTH);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, r[0], r[1], r[0] + r[2], r[1] + r[3]);
  }

  C3D_DrawArrays(GPU_TRIANGLE_STRIP, vertex_count, 4);
  vertex_count += 4;
  ++frame_draws;

  // The OpenGL states again at the next draw.
  DirtyAll();
}

static void ClearWithQuad(GLbitfield mask)
{
  int write = 0;

  if (mask & GL_COLOR_BUFFER_BIT)
  {
    if (color_mask[0]) write |= GPU_WRITE_RED;
    if (color_mask[1]) write |= GPU_WRITE_GREEN;
    if (color_mask[2]) write |= GPU_WRITE_BLUE;
    if (color_mask[3]) write |= GPU_WRITE_ALPHA;
  }

  if ((mask & GL_DEPTH_BUFFER_BIT) && depth_mask)
    write |= GPU_WRITE_DEPTH;

  // The PICA200's depth for the clear value: -(1 - depth).
  ScreenQuad(clear_color, -(1 - clear_depth), write, false,
             (mask & GL_STENCIL_BUFFER_BIT) != 0, scissor_on);
}

void pglBlendScreen(float red, float green, float blue, float alpha)
{
  const float color[4] = {red, green, blue, alpha};

  if (!initialized)
    return;

  BeginFrame();
  ScreenQuad(color, 0, GPU_WRITE_COLOR, true, false, false);
}

void glClear(GLbitfield mask)
{
  bool whole, color, depth, stencil;

  BeginFrame();

  color = (mask & GL_COLOR_BUFFER_BIT) != 0;
  depth = (mask & GL_DEPTH_BUFFER_BIT) != 0;
  stencil = (mask & GL_STENCIL_BUFFER_BIT) != 0;

  whole = !scissor_on
       || (scissor_box[0] <= 0 && scissor_box[1] <= 0
           && scissor_box[0] + scissor_box[2] >= TOP_WIDTH
           && scissor_box[1] + scissor_box[3] >= TOP_HEIGHT);

  // A memory fill is the fastest, but it takes the whole buffers: depth
  // and stencil share one. (A depth clear also clears the stencil.)
  if (whole
      && (!color || (color_mask[0] && color_mask[1] && color_mask[2] && color_mask[3]))
      && (!depth || depth_mask)
      && !(stencil && !depth))
  {
    C3D_ClearBits bits = 0;
    u32 color_value, depth_value;

    if (color)
      bits |= C3D_CLEAR_COLOR;
    if (depth)
      bits |= C3D_CLEAR_DEPTH;

    if (!bits)
      return;

    color_value = ((u32) ColorByte(clear_color[0]) << 24) | ((u32) ColorByte(clear_color[1]) << 16)
                | ((u32) ColorByte(clear_color[2]) << 8) | ColorByte(clear_color[3]);
    depth_value = ((u32) (clear_stencil & 0xFF) << 24)
                | (u32) ((1 - clear_depth) * 0xFFFFFF);

    // After the draws so far: they go to the GPU first.
    C3D_FrameSplit(0);
    C3D_RenderTargetClear(target, bits, color_value, depth_value);
    return;
  }

  ClearWithQuad(mask);
}

// ==========================================================================
// OpenGL: matrices

void glMatrixMode(GLenum mode)
{
  switch (mode)
  {
    case GL_MODELVIEW: matrix_mode = 0; break;
    case GL_PROJECTION: matrix_mode = 1; break;
    case GL_TEXTURE: matrix_mode = 2; break;
    default: SetError(GL_INVALID_ENUM); break;
  }
}

void glLoadIdentity(void)
{
  MatrixIdentity(CurrentMatrix());
  MatrixChanged();
}

void glLoadMatrixf(const GLfloat *m)
{
  memcpy(CurrentMatrix(), m, sizeof(matrix_t));
  MatrixChanged();
}

void glMultMatrixf(const GLfloat *m)
{
  MultiplyCurrent(m);
}

void glPushMatrix(void)
{
  matrix_stack_t *s = &matrices[matrix_mode];

  if (s->depth + 1 >= s->size)
  {
    SetError(GL_STACK_OVERFLOW);
    return;
  }

  memcpy(s->stack[s->depth + 1], s->stack[s->depth], sizeof(matrix_t));
  ++s->depth;
}

void glPopMatrix(void)
{
  matrix_stack_t *s = &matrices[matrix_mode];

  if (s->depth == 0)
  {
    SetError(GL_STACK_UNDERFLOW);
    return;
  }

  --s->depth;
  MatrixChanged();
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
  matrix_t m;

  MatrixIdentity(m);
  m[12] = x;
  m[13] = y;
  m[14] = z;
  MultiplyCurrent(m);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
  matrix_t m;

  MatrixIdentity(m);
  m[0] = x;
  m[5] = y;
  m[10] = z;
  MultiplyCurrent(m);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
  const float length = sqrtf(x * x + y * y + z * z);
  const float radians = angle * (float) M_PI / 180.0f;
  const float c = cosf(radians), s = sinf(radians), ic = 1 - c;
  matrix_t m;

  if (length == 0)
    return;

  x /= length;
  y /= length;
  z /= length;

  MatrixIdentity(m);
  m[0] = x * x * ic + c;     m[4] = x * y * ic - z * s; m[8] = x * z * ic + y * s;
  m[1] = y * x * ic + z * s; m[5] = y * y * ic + c;     m[9] = y * z * ic - x * s;
  m[2] = x * z * ic - y * s; m[6] = y * z * ic + x * s; m[10] = z * z * ic + c;
  MultiplyCurrent(m);
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble near_val, GLdouble far_val)
{
  matrix_t m;

  MatrixIdentity(m);
  m[0] = (float) (2 / (right - left));
  m[5] = (float) (2 / (top - bottom));
  m[10] = (float) (-2 / (far_val - near_val));
  m[12] = (float) (-(right + left) / (right - left));
  m[13] = (float) (-(top + bottom) / (top - bottom));
  m[14] = (float) (-(far_val + near_val) / (far_val - near_val));
  MultiplyCurrent(m);
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble near_val, GLdouble far_val)
{
  matrix_t m;

  memset(m, 0, sizeof(m));
  m[0] = (float) (2 * near_val / (right - left));
  m[5] = (float) (2 * near_val / (top - bottom));
  m[8] = (float) ((right + left) / (right - left));
  m[9] = (float) ((top + bottom) / (top - bottom));
  m[10] = (float) (-(far_val + near_val) / (far_val - near_val));
  m[11] = -1;
  m[14] = (float) (-2 * far_val * near_val / (far_val - near_val));
  MultiplyCurrent(m);
}

// ==========================================================================
// OpenGL: immediate mode

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
  current_color[0] = ColorByte(red);
  current_color[1] = ColorByte(green);
  current_color[2] = ColorByte(blue);
  current_color[3] = ColorByte(alpha);
}

void glColor3f(GLfloat red, GLfloat green, GLfloat blue) { glColor4f(red, green, blue, 1); }
void glColor4fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], v[3]); }
void glColor3fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], 1); }

void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha)
{
  current_color[0] = red;
  current_color[1] = green;
  current_color[2] = blue;
  current_color[3] = alpha;
}

void glColor3ub(GLubyte red, GLubyte green, GLubyte blue) { glColor4ub(red, green, blue, 255); }

void glTexCoord2f(GLfloat s, GLfloat t)
{
  current_s = s;
  current_t = t;
}

void glTexCoord2fv(const GLfloat *v) { glTexCoord2f(v[0], v[1]); }

void glMultiTexCoord2f(GLenum target_, GLfloat s, GLfloat t)
{
  if (target_ == GL_TEXTURE0)
    glTexCoord2f(s, t);
}

void glMultiTexCoord2fv(GLenum target_, const GLfloat *v)
{
  glMultiTexCoord2f(target_, v[0], v[1]);
}

void glBegin(GLenum mode)
{
  BeginFrame();
  begin_mode = mode;
  begin_first = vertex_count;
  in_begin = true;
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
  vertex_t *v;

  if (!in_begin)
    return;

  v = ReserveVertices(1);

  if (!v)
    return;

  v->x = x;
  v->y = y;
  v->z = z;
  v->s = current_s;
  v->t = current_t;
  v->r = current_color[0];
  v->g = current_color[1];
  v->b = current_color[2];
  v->a = current_color[3];
  ++vertex_count;
}

void glVertex2f(GLfloat x, GLfloat y) { glVertex3f(x, y, 0); }
void glVertex2i(GLint x, GLint y) { glVertex3f((GLfloat) x, (GLfloat) y, 0); }
void glVertex3fv(const GLfloat *v) { glVertex3f(v[0], v[1], v[2]); }
void glVertex2fv(const GLfloat *v) { glVertex3f(v[0], v[1], 0); }

void glEnd(void)
{
  if (!in_begin)
    return;

  in_begin = false;
  DrawMode(begin_mode, begin_first, vertex_count - begin_first);
}

// ==========================================================================
// OpenGL: client arrays

static void SetArray(client_array_t *a, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
  a->size = size;
  a->type = type;
  a->stride = stride;
  a->pointer = pointer;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
  SetArray(&vertex_array, size, type, stride, pointer);
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
  SetArray(&color_array, size, type, stride, pointer);
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
  SetArray(&texcoord_array[client_active_texture], size, type, stride, pointer);
}

static const u8 *ArrayElement(const client_array_t *a, int index, int element_size)
{
  const int stride = a->stride ? a->stride : a->size * element_size;

  return (const u8 *) a->pointer + index * stride;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
  vertex_t *out;
  int k;

  if (count <= 0 || !vertex_array.enabled || vertex_array.type != GL_FLOAT)
    return;

  BeginFrame();
  out = ReserveVertices(count);

  if (!out)
    return;

  for (k = 0; k < count; ++k)
  {
    const float *p = (const float *) ArrayElement(&vertex_array, first + k, sizeof(float));

    out[k].x = p[0];
    out[k].y = p[1];
    out[k].z = vertex_array.size > 2 ? p[2] : 0;

    if (texcoord_array[0].enabled && texcoord_array[0].type == GL_FLOAT)
    {
      const float *t = (const float *) ArrayElement(&texcoord_array[0], first + k, sizeof(float));

      out[k].s = t[0];
      out[k].t = texcoord_array[0].size > 1 ? t[1] : 0;
    }
    else
    {
      out[k].s = current_s;
      out[k].t = current_t;
    }

    if (color_array.enabled && color_array.type == GL_UNSIGNED_BYTE)
    {
      const u8 *c = ArrayElement(&color_array, first + k, 1);

      out[k].r = c[0];
      out[k].g = c[1];
      out[k].b = c[2];
      out[k].a = color_array.size > 3 ? c[3] : 255;
    }
    else if (color_array.enabled && color_array.type == GL_FLOAT)
    {
      const float *c = (const float *) ArrayElement(&color_array, first + k, sizeof(float));

      out[k].r = ColorByte(c[0]);
      out[k].g = ColorByte(c[1]);
      out[k].b = ColorByte(c[2]);
      out[k].a = color_array.size > 3 ? ColorByte(c[3]) : 255;
    }
    else
    {
      out[k].r = current_color[0];
      out[k].g = current_color[1];
      out[k].b = current_color[2];
      out[k].a = current_color[3];
    }
  }

  first = vertex_count;
  vertex_count += count;
  DrawMode(mode, first, count);
}

// ==========================================================================
// OpenGL: textures

void glGenTextures(GLsizei n, GLuint *names)
{
  int k, found = 0;

  // Name 0 is "no texture".
  if (texture_count == 0)
  {
    textures = calloc(64, sizeof(*textures));
    texture_count = 64;
    textures[0].used = true;
  }

  for (k = 1; k < texture_count && found < n; ++k)
  {
    if (!textures[k].used)
    {
      memset(&textures[k], 0, sizeof(textures[k]));
      textures[k].used = true;
      names[found++] = k;
    }
  }

  while (found < n)
  {
    const int old = texture_count;

    texture_count *= 2;
    textures = realloc(textures, texture_count * sizeof(*textures));
    memset(textures + old, 0, (texture_count - old) * sizeof(*textures));

    for (k = old; k < texture_count && found < n; ++k)
    {
      textures[k].used = true;
      names[found++] = k;
    }
  }

  for (k = 0; k < n; ++k)
  {
    texture_t *t = &textures[names[k]];

    t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_REPEAT;
  }
}

static void FreeTextureData(texture_t *t)
{
  if (t->has_data)
  {
    FreeLater(t->tex.data);
    t->has_data = false;
  }
}

void glDeleteTextures(GLsizei n, const GLuint *names)
{
  int k, unit;

  for (k = 0; k < n; ++k)
  {
    const GLuint name = names[k];

    if (name == 0 || name >= (GLuint) texture_count)
      continue;

    FreeTextureData(&textures[name]);
    textures[name].used = false;

    for (unit = 0; unit < TEXTURE_UNITS; ++unit)
    {
      if (bound_texture[unit] == name)
      {
        bound_texture[unit] = 0;
        if (unit == 0)
          texture_dirty = true;
      }
    }
  }
}

GLboolean glIsTexture(GLuint name)
{
  return name > 0 && name < (GLuint) texture_count && textures[name].used;
}

void glBindTexture(GLenum target_, GLuint name)
{
  if (name >= (GLuint) texture_count && name != 0)
  {
    SetError(GL_INVALID_VALUE);
    return;
  }

  if (bound_texture[active_texture] != name)
  {
    bound_texture[active_texture] = name;
    if (active_texture == 0)
      texture_dirty = true;
  }

  if (name)
    textures[name].used = true;
}

static texture_t *ActiveTexture(void)
{
  GLuint name = bound_texture[active_texture];

  if (name == 0 || name >= (GLuint) texture_count)
    return NULL;

  return &textures[name];
}

static GPU_TEXTURE_FILTER_PARAM Filter(GLenum filter)
{
  switch (filter)
  {
    case GL_LINEAR:
    case GL_LINEAR_MIPMAP_NEAREST:
    case GL_LINEAR_MIPMAP_LINEAR:
      return GPU_LINEAR;
    default:
      return GPU_NEAREST;
  }
}

static GPU_TEXTURE_WRAP_PARAM Wrap(GLenum wrap)
{
  switch (wrap)
  {
    case GL_CLAMP:
    case GL_CLAMP_TO_EDGE:
      return GPU_CLAMP_TO_EDGE;
    case GL_CLAMP_TO_BORDER:
      return GPU_CLAMP_TO_BORDER;
    case GL_MIRRORED_REPEAT:
      return GPU_MIRRORED_REPEAT;
    default:
      return GPU_REPEAT;
  }
}

static void ApplySampling(texture_t *t)
{
  if (!t->has_data)
    return;

  C3D_TexSetFilter(&t->tex, Filter(t->mag_filter), Filter(t->min_filter));
  C3D_TexSetWrap(&t->tex, Wrap(t->wrap_s), Wrap(t->wrap_t));

  if (t == BoundTexture(0))
    texture_dirty = true;
}

void glTexParameteri(GLenum target_, GLenum name, GLint param)
{
  texture_t *t = ActiveTexture();

  if (!t)
    return;

  switch (name)
  {
    case GL_TEXTURE_MIN_FILTER: t->min_filter = param; break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = param; break;
    case GL_TEXTURE_WRAP_S: t->wrap_s = param; break;
    case GL_TEXTURE_WRAP_T: t->wrap_t = param; break;
    default: return; // mipmap generation, anisotropy...: not here
  }

  ApplySampling(t);
}

void glTexParameterf(GLenum target_, GLenum name, GLfloat param)
{
  glTexParameteri(target_, name, (GLint) param);
}

void glTexEnvi(GLenum target_, GLenum name, GLint param)
{
  if (active_texture != 0)
    return;

  switch (name)
  {
    case GL_TEXTURE_ENV_MODE: env_mode = param; break;
    case GL_COMBINE_RGB: combine_rgb = param; break;
    case GL_COMBINE_ALPHA: combine_alpha = param; break;
    case GL_SOURCE0_RGB: source_rgb[0] = param; break;
    case GL_SOURCE1_RGB: source_rgb[1] = param; break;
    case GL_SOURCE2_RGB: source_rgb[2] = param; break;
    case GL_SOURCE0_ALPHA: source_alpha[0] = param; break;
    case GL_SOURCE1_ALPHA: source_alpha[1] = param; break;
    case GL_SOURCE2_ALPHA: source_alpha[2] = param; break;
    case GL_OPERAND0_RGB: operand_rgb[0] = param; break;
    case GL_OPERAND1_RGB: operand_rgb[1] = param; break;
    case GL_OPERAND2_RGB: operand_rgb[2] = param; break;
    case GL_OPERAND0_ALPHA: operand_alpha[0] = param; break;
    case GL_OPERAND1_ALPHA: operand_alpha[1] = param; break;
    case GL_OPERAND2_ALPHA: operand_alpha[2] = param; break;
    default: return;
  }

  texenv_dirty = true;
}

void glTexEnvf(GLenum target_, GLenum name, GLfloat param)
{
  glTexEnvi(target_, name, (GLint) param);
}

void glTexEnvfv(GLenum target_, GLenum name, const GLfloat *params)
{
  if (name == GL_TEXTURE_ENV_COLOR && active_texture == 0)
  {
    // The combiners take it as ABGR.
    env_color = ((u32) ColorByte(params[3]) << 24) | ((u32) ColorByte(params[2]) << 16)
              | ((u32) ColorByte(params[1]) << 8) | ColorByte(params[0]);
    texenv_dirty = true;
  }
  else
  {
    glTexEnvi(target_, name, (GLint) params[0]);
  }
}

void glTexGenf(GLenum coord, GLenum name, GLfloat param)
{
  // Only GL_EYE_LINEAR, the default and all DSDA-Doom uses.
}

void glTexGeni(GLenum coord, GLenum name, GLint param) {}

void glTexGenfv(GLenum coord, GLenum name, const GLfloat *params)
{
  const int k = coord - GL_S;

  // OpenGL keeps eye planes times the inverse of the modelview matrix at
  // the time; DSDA-Doom sets them with the identity there.
  if (name == GL_EYE_PLANE && k >= 0 && k < 4)
  {
    memcpy(texgen_plane[k], params, sizeof(texgen_plane[k]));
    texgen_dirty = true;
  }
}

// Texels: the PICA200 keeps textures in 8x8 tiles, the pixels of each in
// Z order. Position of the pixel at x, y in a tile:
static const u8 tile_order[64] = {
   0,  1,  4,  5, 16, 17, 20, 21,
   2,  3,  6,  7, 18, 19, 22, 23,
   8,  9, 12, 13, 24, 25, 28, 29,
  10, 11, 14, 15, 26, 27, 30, 31,
  32, 33, 36, 37, 48, 49, 52, 53,
  34, 35, 38, 39, 50, 51, 54, 55,
  40, 41, 44, 45, 56, 57, 60, 61,
  42, 43, 46, 47, 58, 59, 62, 63,
};

static inline u32 TexelOffset(int x, int y, int width)
{
  return ((y >> 3) * (width >> 3) + (x >> 3)) * 64 + tile_order[(y & 7) * 8 + (x & 7)];
}

static int BytesPerPixel(GLenum format)
{
  switch (format)
  {
    case GL_RGBA:
    case GL_BGRA:
      return 4;
    case GL_RGB:
    case GL_BGR:
      return 3;
    case GL_RG:
    case GL_LUMINANCE_ALPHA:
      return 2;
    default: // GL_RED, GL_LUMINANCE, GL_ALPHA, GL_INTENSITY
      return 1;
  }
}

// A pixel in the PICA200's RGBA8: 0xRRGGBBAA.
static inline u32 TexelFrom(const u8 *p, GLenum format)
{
  switch (format)
  {
    case GL_RGBA: return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
    case GL_BGRA: return ((u32) p[2] << 24) | ((u32) p[1] << 16) | ((u32) p[0] << 8) | p[3];
    case GL_RGB: return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | 0xFF;
    case GL_BGR: return ((u32) p[2] << 24) | ((u32) p[1] << 16) | ((u32) p[0] << 8) | 0xFF;
    case GL_RG: return ((u32) p[0] << 24) | ((u32) p[1] << 16) | 0xFF;
    case GL_LUMINANCE_ALPHA: return ((u32) p[0] * 0x01010100) | p[1];
    case GL_ALPHA: return p[0];
    case GL_RED: return ((u32) p[0] << 24) | 0xFF;
    default: return ((u32) p[0] * 0x01010100) | 0xFF; // luminance
  }
}

static int PowerOfTwo(int size)
{
  int p = MIN_TEXTURE_SIZE;

  while (p < size && p < MAX_TEXTURE_SIZE)
    p *= 2;

  return p;
}

// Copies a width x height image at xoffset, yoffset of t's image (its
// OpenGL size), into the texels: each texel takes the nearest pixel.
static void WriteTexels(texture_t *t, int xoffset, int yoffset, int width, int height,
                        GLenum format, const void *pixels)
{
  const int tex_width = t->tex.width, tex_height = t->tex.height;
  const int bpp = BytesPerPixel(format);
  const int pitch = (width * bpp + unpack_alignment - 1) / unpack_alignment * unpack_alignment;
  u32 *texels = t->tex.data;
  int x, y;

  for (y = 0; y < tex_height; ++y)
  {
    const int sy = y * t->height / tex_height - yoffset;
    const u8 *row;

    if (sy < 0 || sy >= height)
      continue;

    row = (const u8 *) pixels + sy * pitch;

    for (x = 0; x < tex_width; ++x)
    {
      const int sx = x * t->width / tex_width - xoffset;

      if (sx < 0 || sx >= width)
        continue;

      texels[TexelOffset(x, y, tex_width)] = TexelFrom(row + sx * bpp, format);
    }
  }
}

void glTexImage2D(GLenum target_, GLint level, GLint internal_format, GLsizei width,
                  GLsizei height, GLint border, GLenum format, GLenum type,
                  const GLvoid *pixels)
{
  texture_t *t = ActiveTexture();
  const int tex_width = PowerOfTwo(width), tex_height = PowerOfTwo(height);

  // One level: no mipmaps.
  if (!t || level != 0)
    return;

  if (type != GL_UNSIGNED_BYTE)
  {
    SetError(GL_INVALID_ENUM);
    return;
  }

  // New memory each time: the GPU may still draw with the old one.
  FreeTextureData(t);

  if (!C3D_TexInit(&t->tex, tex_width, tex_height, GPU_RGBA8))
  {
    printf("pgl: no memory for a %dx%d texture\n", tex_width, tex_height);
    SetError(GL_OUT_OF_MEMORY);
    return;
  }

  t->has_data = true;
  t->width = width;
  t->height = height;

  if (pixels)
    WriteTexels(t, 0, 0, width, height, format, pixels);
  else
    memset(t->tex.data, 0, t->tex.size);

  ApplySampling(t);

  if (t == BoundTexture(0))
    texture_dirty = true;
}

void glTexSubImage2D(GLenum target_, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const GLvoid *pixels)
{
  texture_t *t = ActiveTexture();

  if (!t || !t->has_data || level != 0 || type != GL_UNSIGNED_BYTE)
    return;

  // Drawn with in this frame: a copy to change, the GPU keeps the old one.
  if (t->frame == frame_number)
  {
    void *old = t->tex.data;
    void *copy = linearAlloc(t->tex.size);

    if (!copy)
      return;

    memcpy(copy, old, t->tex.size);
    FreeLater(old);
    t->tex.data = copy;

    if (t == BoundTexture(0))
      texture_dirty = true;
  }

  WriteTexels(t, xoffset, yoffset, width, height, format, pixels);
}

void glGetTexLevelParameteriv(GLenum target_, GLint level, GLenum name, GLint *params)
{
  texture_t *t = ActiveTexture();

  if (!t || !t->has_data || level != 0)
  {
    *params = 0;
    return;
  }

  switch (name)
  {
    case GL_TEXTURE_WIDTH: *params = t->width; break;
    case GL_TEXTURE_HEIGHT: *params = t->height; break;
    case GL_TEXTURE_INTERNAL_FORMAT: *params = GL_RGBA; break;
    default: *params = 0; break;
  }
}

void glGetTexImage(GLenum target_, GLint level, GLenum format, GLenum type, GLvoid *pixels)
{
  texture_t *t = ActiveTexture();
  u8 *out = pixels;
  int x, y;

  if (!t || !t->has_data || level != 0 || format != GL_RGBA || type != GL_UNSIGNED_BYTE)
    return;

  for (y = 0; y < t->height; ++y)
  {
    for (x = 0; x < t->width; ++x)
    {
      const int tx = x * t->tex.width / t->width;
      const int ty = y * t->tex.height / t->height;
      const u32 texel = ((u32 *) t->tex.data)[TexelOffset(tx, ty, t->tex.width)];

      *out++ = texel >> 24;
      *out++ = texel >> 16;
      *out++ = texel >> 8;
      *out++ = texel;
    }
  }
}

// ==========================================================================
// OpenGL: reading the image (screenshots, screen wipes)

// The pixel at x, y of the screen (from the bottom left), from the target
// once the GPU has drawn it: 0xRRGGBBAA.
static u32 ReadPixel(const u32 *color_buffer, int x, int y)
{
  const int rx = y;
  const int ry = TOP_WIDTH - 1 - x;

  return color_buffer[TexelOffset(rx, ry, TOP_HEIGHT)];
}

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                  GLenum type, GLvoid *pixels)
{
  const int bpp = format == GL_RGBA ? 4 : 3;
  const int pitch = (width * bpp + pack_alignment - 1) / pack_alignment * pack_alignment;
  const u32 *color_buffer;
  int i, j;

  if (!initialized || type != GL_UNSIGNED_BYTE || (format != GL_RGB && format != GL_RGBA))
    return;

  FinishDrawing();
  color_buffer = (const u32 *) target->frameBuf.colorBuf;

  for (j = 0; j < height; ++j)
  {
    u8 *row = (u8 *) pixels + j * pitch;

    for (i = 0; i < width; ++i)
    {
      const int sx = x + i, sy = y + j;
      u32 pixel = 0;

      if (sx >= 0 && sx < TOP_WIDTH && sy >= 0 && sy < TOP_HEIGHT)
        pixel = ReadPixel(color_buffer, sx, sy);

      row[i * bpp + 0] = pixel >> 24;
      row[i * bpp + 1] = pixel >> 16;
      row[i * bpp + 2] = pixel >> 8;
      if (bpp == 4)
        row[i * bpp + 3] = pixel;
    }
  }
}

void glCopyTexSubImage2D(GLenum target_, GLint level, GLint xoffset, GLint yoffset,
                         GLint x, GLint y, GLsizei width, GLsizei height)
{
  texture_t *t = ActiveTexture();
  u32 *pixels;

  if (!t || !t->has_data || level != 0 || width <= 0 || height <= 0)
    return;

  pixels = malloc(width * height * 4);

  if (!pixels)
    return;

  {
    const int saved = pack_alignment;

    pack_alignment = 1;
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    pack_alignment = saved;
  }

  {
    const int saved = unpack_alignment;

    unpack_alignment = 1;
    glTexSubImage2D(target_, level, xoffset, yoffset, width, height, GL_RGBA,
                    GL_UNSIGNED_BYTE, pixels);
    unpack_alignment = saved;
  }

  free(pixels);
}

// ==========================================================================
// Extensions

void glActiveTextureARB(GLenum texture) { glActiveTexture(texture); }
void glClientActiveTextureARB(GLenum texture) { glClientActiveTexture(texture); }
void glMultiTexCoord2fARB(GLenum target_, GLfloat s, GLfloat t) { glMultiTexCoord2f(target_, s, t); }
void glMultiTexCoord2fvARB(GLenum target_, const GLfloat *v) { glMultiTexCoord2fv(target_, v); }

void *pglGetProcAddress(const char *name)
{
  static const struct
  {
    const char *name;
    void *function;
  } functions[] = {
    {"glActiveTextureARB", (void *) glActiveTextureARB},
    {"glClientActiveTextureARB", (void *) glClientActiveTextureARB},
    {"glMultiTexCoord2fARB", (void *) glMultiTexCoord2fARB},
    {"glMultiTexCoord2fvARB", (void *) glMultiTexCoord2fvARB},
    {"glActiveTexture", (void *) glActiveTexture},
    {"glClientActiveTexture", (void *) glClientActiveTexture},
    {"glMultiTexCoord2f", (void *) glMultiTexCoord2f},
    {"glMultiTexCoord2fv", (void *) glMultiTexCoord2fv},
  };
  size_t k;

  for (k = 0; k < sizeof(functions) / sizeof(functions[0]); ++k)
    if (!strcmp(functions[k].name, name))
      return functions[k].function;

  return NULL;
}
