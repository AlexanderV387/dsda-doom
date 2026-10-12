//
// [3DS] GLU's polygon tessellator, on top of libtess2 (the same SGI code,
// cleaned up): DSDA-Doom's OpenGL renderer cuts each sector's floor and
// ceiling into triangles with it when a level starts.
//
// GLU takes the vertices one by one and calls back with the triangles;
// libtess2 takes whole contours and returns arrays. So the vertices are
// collected until gluTessEndPolygon, then tessellated at once, and the
// triangles go to the callbacks as GL_TRIANGLES.
//

#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/glu.h>

#include "libtess2/Include/tesselator.h"

typedef void (*begin_callback_t)(GLenum type);
typedef void (*vertex_callback_t)(void *data);
typedef void (*end_callback_t)(void);
typedef void (*error_callback_t)(GLenum error);
typedef void (*combine_callback_t)(GLdouble coords[3], void *data[4], GLfloat weight[4],
                                   void **out);

struct GLUtesselator
{
  begin_callback_t begin;
  vertex_callback_t vertex;
  end_callback_t end;
  error_callback_t error;
  combine_callback_t combine;
  GLenum winding;

  // The polygon so far: every vertex, and where each contour starts.
  float *coords;     // x, y, z
  void **data;       // gluTessVertex's data
  int count, size;
  int *contours;     // first vertex of each
  int contour_count, contour_size;
};

GLUtesselator *gluNewTess(void)
{
  GLUtesselator *tess = calloc(1, sizeof(*tess));

  if (tess)
    tess->winding = GLU_TESS_WINDING_ODD;

  return tess;
}

void gluDeleteTess(GLUtesselator *tess)
{
  if (!tess)
    return;

  free(tess->coords);
  free(tess->data);
  free(tess->contours);
  free(tess);
}

void gluTessCallback(GLUtesselator *tess, GLenum which, _GLUfuncptr callback)
{
  switch (which)
  {
    case GLU_TESS_BEGIN: tess->begin = (begin_callback_t) callback; break;
    case GLU_TESS_VERTEX: tess->vertex = (vertex_callback_t) callback; break;
    case GLU_TESS_END: tess->end = (end_callback_t) callback; break;
    case GLU_TESS_ERROR: tess->error = (error_callback_t) callback; break;
    case GLU_TESS_COMBINE: tess->combine = (combine_callback_t) callback; break;
    default: break; // edge flags and the *_DATA forms: not used
  }
}

void gluTessProperty(GLUtesselator *tess, GLenum which, GLdouble data)
{
  if (which == GLU_TESS_WINDING_RULE)
    tess->winding = (GLenum) data;
}

void gluTessNormal(GLUtesselator *tess, GLdouble x, GLdouble y, GLdouble z) {}

void gluTessBeginPolygon(GLUtesselator *tess, GLvoid *data)
{
  tess->count = 0;
  tess->contour_count = 0;
}

void gluTessBeginContour(GLUtesselator *tess)
{
  if (tess->contour_count == tess->contour_size)
  {
    tess->contour_size = tess->contour_size ? tess->contour_size * 2 : 16;
    tess->contours = realloc(tess->contours, tess->contour_size * sizeof(*tess->contours));
  }

  tess->contours[tess->contour_count++] = tess->count;
}

void gluTessVertex(GLUtesselator *tess, GLdouble *location, GLvoid *data)
{
  if (tess->count == tess->size)
  {
    tess->size = tess->size ? tess->size * 2 : 64;
    tess->coords = realloc(tess->coords, tess->size * 3 * sizeof(*tess->coords));
    tess->data = realloc(tess->data, tess->size * sizeof(*tess->data));
  }

  tess->coords[tess->count * 3 + 0] = (float) location[0];
  tess->coords[tess->count * 3 + 1] = (float) location[1];
  tess->coords[tess->count * 3 + 2] = (float) location[2];
  tess->data[tess->count] = data;
  ++tess->count;
}

void gluTessEndContour(GLUtesselator *tess) {}

void gluNextContour(GLUtesselator *tess, GLenum type)
{
  gluTessBeginContour(tess);
}

static int WindingRule(GLenum winding)
{
  switch (winding)
  {
    case GLU_TESS_WINDING_NONZERO: return TESS_WINDING_NONZERO;
    case GLU_TESS_WINDING_POSITIVE: return TESS_WINDING_POSITIVE;
    case GLU_TESS_WINDING_NEGATIVE: return TESS_WINDING_NEGATIVE;
    case GLU_TESS_WINDING_ABS_GEQ_TWO: return TESS_WINDING_ABS_GEQ_TWO;
    default: return TESS_WINDING_ODD;
  }
}

// A vertex libtess2 made (where edges cross): GLU asks the program for its
// data through the combine callback, with the input vertices around it.
// libtess2 does not say which ones; the nearest input vertex stands for
// them (DSDA-Doom keeps the first anyway).
static void *CombinedData(GLUtesselator *tess, const float *position)
{
  void *neighbors[4] = {NULL, NULL, NULL, NULL};
  GLfloat weight[4] = {1, 0, 0, 0};
  GLdouble coords[3];
  void *out = NULL;
  float best = -1;
  int k;

  for (k = 0; k < tess->count; ++k)
  {
    const float *c = &tess->coords[k * 3];
    const float dx = c[0] - position[0], dy = c[1] - position[1], dz = c[2] - position[2];
    const float distance = dx * dx + dy * dy + dz * dz;

    if (best < 0 || distance < best)
    {
      best = distance;
      neighbors[0] = tess->data[k];
    }
  }

  if (!tess->combine)
    return neighbors[0];

  coords[0] = position[0];
  coords[1] = position[1];
  coords[2] = position[2];
  tess->combine(coords, neighbors, weight, &out);
  return out;
}

void gluTessEndPolygon(GLUtesselator *tess)
{
  static const TESSreal normal[3] = {0, 1, 0}; // DSDA-Doom's sectors lie in x, z
  TESStesselator *t;
  const TESSindex *elements, *indices;
  const TESSreal *positions;
  int k, i, triangles;

  if (tess->count < 3)
    return;

  t = tessNewTess(NULL);

  if (!t)
  {
    if (tess->error)
      tess->error(GLU_OUT_OF_MEMORY);
    return;
  }

  for (k = 0; k < tess->contour_count; ++k)
  {
    const int first = tess->contours[k];
    const int last = k + 1 < tess->contour_count ? tess->contours[k + 1] : tess->count;

    if (last - first >= 2)
      tessAddContour(t, 3, &tess->coords[first * 3], 3 * sizeof(float), last - first);
  }

  // Sectors with nothing but a contour of their own outline: no contours,
  // as if it were one (GLU allows a missing gluTessBeginContour).
  if (tess->contour_count == 0)
    tessAddContour(t, 3, tess->coords, 3 * sizeof(float), tess->count);

  if (!tessTesselate(t, WindingRule(tess->winding), TESS_POLYGONS, 3, 3, normal))
  {
    if (tess->error)
      tess->error(GLU_OUT_OF_MEMORY);
    tessDeleteTess(t);
    return;
  }

  triangles = tessGetElementCount(t);
  elements = tessGetElements(t);
  indices = tessGetVertexIndices(t);
  positions = tessGetVertices(t);

  if (triangles > 0)
  {
    if (tess->begin)
      tess->begin(GL_TRIANGLES);

    for (k = 0; k < triangles; ++k)
    {
      for (i = 0; i < 3; ++i)
      {
        const TESSindex v = elements[k * 3 + i];
        void *data;

        if (v == TESS_UNDEF)
          continue;

        if (indices[v] != TESS_UNDEF)
          data = tess->data[indices[v]];
        else
          data = CombinedData(tess, &positions[v * 3]);

        if (tess->vertex)
          tess->vertex(data);
      }
    }

    if (tess->end)
      tess->end();
  }

  tessDeleteTess(t);
}

const GLubyte *gluErrorString(GLenum error)
{
  switch (error)
  {
    case GLU_INVALID_ENUM: return (const GLubyte *) "invalid enumerant";
    case GLU_INVALID_VALUE: return (const GLubyte *) "invalid value";
    case GLU_OUT_OF_MEMORY: return (const GLubyte *) "out of memory";
    default: return (const GLubyte *) "unknown error";
  }
}
