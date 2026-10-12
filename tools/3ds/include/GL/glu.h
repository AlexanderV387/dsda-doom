/*
 * [3DS] The part of GLU that DSDA-Doom uses: the polygon tessellator,
 * implemented in prboom2/src/n3ds/pgl_glu.c on top of libtess2. The values
 * are GLU 1.3's.
 */

#ifndef __glu_h__
#define __glu_h__

#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GLUtesselator GLUtesselator;
typedef GLUtesselator GLUtesselatorObj;
typedef GLUtesselator GLUtriangulatorObj;
typedef void (*_GLUfuncptr)(void);

#define GLU_INVALID_ENUM 100900
#define GLU_INVALID_VALUE 100901
#define GLU_OUT_OF_MEMORY 100902

#define GLU_TESS_BEGIN 100100
#define GLU_BEGIN 100100
#define GLU_TESS_VERTEX 100101
#define GLU_VERTEX 100101
#define GLU_TESS_END 100102
#define GLU_END 100102
#define GLU_TESS_ERROR 100103
#define GLU_TESS_EDGE_FLAG 100104
#define GLU_EDGE_FLAG 100104
#define GLU_TESS_COMBINE 100105
#define GLU_TESS_BEGIN_DATA 100106
#define GLU_TESS_VERTEX_DATA 100107
#define GLU_TESS_END_DATA 100108
#define GLU_TESS_ERROR_DATA 100109
#define GLU_TESS_EDGE_FLAG_DATA 100110
#define GLU_TESS_COMBINE_DATA 100111

#define GLU_CW 100120
#define GLU_CCW 100121
#define GLU_INTERIOR 100122
#define GLU_EXTERIOR 100123
#define GLU_UNKNOWN 100124

#define GLU_TESS_WINDING_RULE 100140
#define GLU_TESS_BOUNDARY_ONLY 100141
#define GLU_TESS_TOLERANCE 100142

#define GLU_TESS_WINDING_ODD 100130
#define GLU_TESS_WINDING_NONZERO 100131
#define GLU_TESS_WINDING_POSITIVE 100132
#define GLU_TESS_WINDING_NEGATIVE 100133
#define GLU_TESS_WINDING_ABS_GEQ_TWO 100134

GLUtesselator *gluNewTess(void);
void gluDeleteTess(GLUtesselator *tess);
void gluTessCallback(GLUtesselator *tess, GLenum which, _GLUfuncptr callback);
void gluTessProperty(GLUtesselator *tess, GLenum which, GLdouble data);
void gluTessNormal(GLUtesselator *tess, GLdouble x, GLdouble y, GLdouble z);
void gluTessBeginPolygon(GLUtesselator *tess, GLvoid *data);
void gluTessBeginContour(GLUtesselator *tess);
void gluTessVertex(GLUtesselator *tess, GLdouble *location, GLvoid *data);
void gluTessEndContour(GLUtesselator *tess);
void gluTessEndPolygon(GLUtesselator *tess);
void gluNextContour(GLUtesselator *tess, GLenum type);
const GLubyte *gluErrorString(GLenum error);

#ifdef __cplusplus
}
#endif

#endif
